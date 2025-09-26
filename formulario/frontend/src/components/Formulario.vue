<template>
  <form @submit.prevent="enviar">
    <input v-model="nombre" placeholder="Nombre" />
    <input v-model="email" placeholder="Email" type="email" />
    <button type="submit">enviar</button>
  </form>
</template>

<script>
export default {
  data() {
    return {
      nombre: '',
      email: ''
    }
  },
  methods: {
    async enviar() {
      const res = await fetch('localhost:3000/api/form', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ nombre: this.nombre, email: this.email })
      });
      const data = await res.json();
      console.log(data);
    }
  }
}
</script>
